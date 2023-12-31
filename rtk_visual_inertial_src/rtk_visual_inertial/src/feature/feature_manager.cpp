

#include "feature_manager.h"
#include "../parameter/parameters.h"



int FeaturePerId::endFrame() {
    return start_frame + feature_per_frame.size() - 1;
}

FeatureManager::FeatureManager(Eigen::Matrix3d _Rs[])
    : Rs(_Rs) {
    for (int i = 0; i < NUM_OF_CAM; i++)
        ric[i].setIdentity();
}

void FeatureManager::setRic(Eigen::Matrix3d _ric[]) {
    for (int i = 0; i < NUM_OF_CAM; i++) {
        ric[i] = _ric[i];
    }
}

void FeatureManager::ClearState() {
    feature.clear();
}




bool FeatureManager::addFeatureCheckParallax(int image_index, const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>>& image) {

    last_track_num = 0;
    last_average_parallax = 0;
    new_feature_num = 0;
    long_track_num = 0;
    for (auto& id_pts : image) {
        FeaturePerFrame f_per_fra(id_pts.second[0].second);
        ASSERT(id_pts.second[0].first == 0);
        if (id_pts.second.size() == 2) {
            f_per_fra.rightObservation(id_pts.second[1].second);
            ASSERT(id_pts.second[1].first == 1);
        }

        int feature_id = id_pts.first;
        auto it = find_if(feature.begin(), feature.end(), [feature_id](const FeaturePerId & it) {
            return it.feature_id == feature_id;
        });

        if (it == feature.end()) {
            feature.push_back(FeaturePerId(feature_id, image_index));
            feature.back().feature_per_frame.push_back(f_per_fra);
            new_feature_num++;
        } else if (it->feature_id == feature_id) {
            it->feature_per_frame.push_back(f_per_fra);
            last_track_num++;
            if ( it-> feature_per_frame.size() >= 4)
                long_track_num++;
        }
    }

    if (image_index < 2 || last_track_num < 20 || long_track_num < 40 || new_feature_num > 0.5 * last_track_num)
        return true;
    else
        return false;

}



bool FeatureManager::CheckParallax(int image_index) {
    double parallax_sum = 0;
    int parallax_num = 0;

    // std::cout<<"num feature:"<<feature.size()<<std::endl;
    for (auto& it_per_id : feature) {
        if (it_per_id.start_frame <= image_index - 2 &&
                it_per_id.start_frame + int(it_per_id.feature_per_frame.size()) - 1 >= image_index - 1) {
            parallax_sum += compensatedParallax2(it_per_id, image_index);
            parallax_num++;
        }
    }

    if (parallax_num == 0) {
        return true;
    } else {
        last_average_parallax = parallax_sum / parallax_num * FOCAL_LENGTH;
        return parallax_sum / parallax_num >= MIN_PARALLAX;
    }
}

vector<pair<Eigen::Vector3d, Eigen::Vector3d>> FeatureManager::getCorresponding(int frame_count_l, int frame_count_r) {
    vector<pair<Eigen::Vector3d, Eigen::Vector3d>> corres;
    for (auto& it : feature) {
        if (it.start_frame <= frame_count_l && it.endFrame() >= frame_count_r) {
            Eigen::Vector3d a = Eigen::Vector3d::Zero(), b = Eigen::Vector3d::Zero();
            int idx_l = frame_count_l - it.start_frame;
            int idx_r = frame_count_r - it.start_frame;

            a = it.feature_per_frame[idx_l].point;

            b = it.feature_per_frame[idx_r].point;

            corres.push_back(make_pair(a, b));
        }
    }
    return corres;
}


void FeatureManager::removeFailures(ceres::Problem& my_problem) {
    for (auto it = feature.begin(), it_next = feature.begin();
            it != feature.end(); it = it_next) {
        it_next++;
        if (it->solve_flag == 2) {
            if (USE_GLOBAL_OPTIMIZATION) {
#if USE_INVERSE_DEPTH
                if (my_problem.HasParameterBlock(&it->idepth_))
                    my_problem.RemoveParameterBlock(&it->idepth_);
#else
                if (my_problem.HasParameterBlock(it->ptsInWorld.data()))
                    my_problem.RemoveParameterBlock(it->ptsInWorld.data());
#endif
            }
            feature.erase(it);
        }
    }
}

void FeatureManager::clearDepth() {
    for (auto& it_per_id : feature)
        it_per_id.valid = false;
}



void FeatureManager::triangulatePoint(Eigen::Matrix<double, 3, 4>& Pose0, Eigen::Matrix<double, 3, 4>& Pose1,
                                      Eigen::Vector2d& point0, Eigen::Vector2d& point1, Eigen::Vector3d& point_3d) {
    Eigen::Matrix4d design_matrix = Eigen::Matrix4d::Zero();
    design_matrix.row(0) = point0[0] * Pose0.row(2) - Pose0.row(0);
    design_matrix.row(1) = point0[1] * Pose0.row(2) - Pose0.row(1);
    design_matrix.row(2) = point1[0] * Pose1.row(2) - Pose1.row(0);
    design_matrix.row(3) = point1[1] * Pose1.row(2) - Pose1.row(1);
    Eigen::Vector4d triangulated_point;
    triangulated_point =
        design_matrix.jacobiSvd(Eigen::ComputeFullV).matrixV().rightCols<1>();
    point_3d(0) = triangulated_point(0) / triangulated_point(3);
    point_3d(1) = triangulated_point(1) / triangulated_point(3);
    point_3d(2) = triangulated_point(2) / triangulated_point(3);
}


bool FeatureManager::solvePoseByPnP(Eigen::Matrix3d& R, Eigen::Vector3d& P,
                                    vector<cv::Point2f>& pts2D, vector<cv::Point3f>& pts3D) {
    Eigen::Matrix3d R_initial;
    Eigen::Vector3d P_initial;

    // w_T_cam ---> cam_T_w
    R_initial = R.inverse();
    P_initial = -(R_initial * P);

    //printf("pnp size %d \n",(int)pts2D.size() );
    if (int(pts2D.size()) < 4) {
        printf("feature tracking not enough, please slowly move you device! \n");
        return false;
    }
    cv::Mat r, rvec, t, D, tmp_r;
    cv::eigen2cv(R_initial, tmp_r);
    cv::Rodrigues(tmp_r, rvec);
    cv::eigen2cv(P_initial, t);
    cv::Mat K = (cv::Mat_<double>(3, 3) << 1, 0, 0, 0, 1, 0, 0, 0, 1);
    bool pnp_succ;
    // pnp_succ = cv::solvePnP(pts3D, pts2D, K, D, rvec, t, 1);
    pnp_succ = solvePnPRansac(pts3D, pts2D, K, D, rvec, t, true, 100, 8.0 / FOCAL_LENGTH, 0.99);

    if (!pnp_succ) {
        printf("pnp failed ! \n");
        return false;
    }
    cv::Rodrigues(rvec, r);
    //cout << "r " << endl << r << endl;
    Eigen::MatrixXd R_pnp;
    cv::cv2eigen(r, R_pnp);
    Eigen::MatrixXd T_pnp;
    cv::cv2eigen(t, T_pnp);

    // cam_T_w ---> w_T_cam
    R = R_pnp.transpose();
    P = R * (-T_pnp);

    return true;
}

void FeatureManager::initFramePoseByPnP(int frameCnt, Eigen::Vector3d Ps[], Eigen::Matrix3d Rs[], Eigen::Vector3d tic[], Eigen::Matrix3d ric[], Eigen::Vector3d Pbg_) {

    if (frameCnt > 0) {
        vector<cv::Point2f> pts2D;
        vector<cv::Point3f> pts3D;
        for (auto& it_per_id : feature) {
            if (it_per_id.valid) {
                int index = frameCnt - it_per_id.start_frame;
#if USE_INVERSE_DEPTH
                Eigen::Vector3d ptsInW = Rs[it_per_id.start_frame] * (
                                             ric[0] * (it_per_id.feature_per_frame[0].point / it_per_id.idepth_) + tic[0] - Pbg_
                                         ) + Ps[it_per_id.start_frame];
#else
                Eigen::Vector3d ptsInW = it_per_id.ptsInWorld;
#endif

                if ((int)it_per_id.feature_per_frame.size() >= index + 1) {
                    cv::Point3f point3d(ptsInW.x(), ptsInW.y(), ptsInW.z());
                    cv::Point2f point2d(it_per_id.feature_per_frame[index].point.x(), it_per_id.feature_per_frame[index].point.y());
                    pts3D.push_back(point3d);
                    pts2D.push_back(point2d);
                }
            }
        }
        Eigen::Matrix3d RCam;
        Eigen::Vector3d PCam;
        // trans to w_T_cam
        RCam = Rs[frameCnt - 1] * ric[0];
        PCam = Rs[frameCnt - 1] * (tic[0] - Pbg_) + Ps[frameCnt - 1];

        if (solvePoseByPnP(RCam, PCam, pts2D, pts3D)) {
            // trans to w_T_imu
            Rs[frameCnt] = RCam * ric[0].transpose();
            Ps[frameCnt] = -RCam * ric[0].transpose() * (tic[0] - Pbg_) + PCam;

            Eigen::Quaterniond Q(Rs[frameCnt]);
        }
    }
}

void FeatureManager::triangulate(Eigen::Vector3d Ps[], Eigen::Matrix3d Rs[], Eigen::Vector3d tic[], Eigen::Matrix3d ric[], Eigen::Vector3d Pbg_) {
    for (auto& it_per_id : feature) {
        if (it_per_id.valid)
            continue;

        if ( it_per_id.feature_per_frame[0].is_stereo) {
            int imu_i = it_per_id.start_frame;
            Eigen::Matrix<double, 3, 4> leftPose;
            Eigen::Vector3d t0 = Ps[imu_i] + Rs[imu_i] * tic[0];
            Eigen::Matrix3d R0 = Rs[imu_i] * ric[0];
            leftPose.leftCols<3>() = R0.transpose();
            leftPose.rightCols<1>() = -R0.transpose() * t0;
            //cout << "left pose " << leftPose << endl;

            Eigen::Matrix<double, 3, 4> rightPose;
            Eigen::Vector3d t1 = Ps[imu_i] + Rs[imu_i] * tic[1];
            Eigen::Matrix3d R1 = Rs[imu_i] * ric[1];
            rightPose.leftCols<3>() = R1.transpose();
            rightPose.rightCols<1>() = -R1.transpose() * t1;
            //cout << "right pose " << rightPose << endl;

            Eigen::Vector2d point0, point1;
            Eigen::Vector3d point3d;
            point0 = it_per_id.feature_per_frame[0].point.head(2);
            point1 = it_per_id.feature_per_frame[0].pointRight.head(2);
            //cout << "point0 " << point0.transpose() << endl;
            //cout << "point1 " << point1.transpose() << endl;

            triangulatePoint(leftPose, rightPose, point0, point1, point3d);
            Eigen::Vector3d localPoint;
            localPoint = leftPose.leftCols<3>() * point3d + leftPose.rightCols<1>();
            double depth = localPoint.z();
            if (depth <= 0)
                depth = INIT_DEPTH;
            it_per_id.idepth_ = 1. / depth;
            it_per_id.ptsInWorld = Rs[it_per_id.start_frame] * (
                                       ric[0] * (it_per_id.feature_per_frame[0].point / it_per_id.idepth_) + tic[0] - Pbg_
                                   ) + Ps[it_per_id.start_frame];
            it_per_id.valid = true;
            continue;
        } else if (it_per_id.feature_per_frame.size() > 1) {
            int imu_i = it_per_id.start_frame;
            Eigen::Matrix<double, 3, 4> leftPose;
            Eigen::Vector3d t0 = Ps[imu_i] + Rs[imu_i] * tic[0];
            Eigen::Matrix3d R0 = Rs[imu_i] * ric[0];
            leftPose.leftCols<3>() = R0.transpose();
            leftPose.rightCols<1>() = -R0.transpose() * t0;

            imu_i++;
            Eigen::Matrix<double, 3, 4> rightPose;
            Eigen::Vector3d t1 = Ps[imu_i] + Rs[imu_i] * tic[0];
            Eigen::Matrix3d R1 = Rs[imu_i] * ric[0];
            rightPose.leftCols<3>() = R1.transpose();
            rightPose.rightCols<1>() = -R1.transpose() * t1;

            Eigen::Vector2d point0, point1;
            Eigen::Vector3d point3d;
            point0 = it_per_id.feature_per_frame[0].point.head(2);
            point1 = it_per_id.feature_per_frame[1].point.head(2);
            triangulatePoint(leftPose, rightPose, point0, point1, point3d);
            Eigen::Vector3d localPoint;
            localPoint = leftPose.leftCols<3>() * point3d + leftPose.rightCols<1>();
            double depth = localPoint.z();
            if (depth <= 0)
                depth = INIT_DEPTH;
            it_per_id.idepth_ = 1. / depth;
            it_per_id.ptsInWorld = Rs[it_per_id.start_frame] * (
                                       ric[0] * (it_per_id.feature_per_frame[0].point / it_per_id.idepth_) + tic[0] - Pbg_
                                   ) + Ps[it_per_id.start_frame];
            it_per_id.valid = true;
            continue;
        }
        if (it_per_id.feature_per_frame.size() < FEATURE_CONTINUE || it_per_id.start_frame >= FEATURE_WINDOW_SIZE - 2)
            continue;

        int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;

        Eigen::MatrixXd svd_A(2 * it_per_id.feature_per_frame.size(), 4);
        int svd_idx = 0;

        Eigen::Matrix<double, 3, 4> P0;
        Eigen::Vector3d t0 = Ps[imu_i] + Rs[imu_i] * tic[0];
        Eigen::Matrix3d R0 = Rs[imu_i] * ric[0];
        P0.leftCols<3>() = Eigen::Matrix3d::Identity();
        P0.rightCols<1>() = Eigen::Vector3d::Zero();

        for (auto& it_per_frame : it_per_id.feature_per_frame) {
            imu_j++;

            Eigen::Vector3d t1 = Ps[imu_j] + Rs[imu_j] * tic[0];
            Eigen::Matrix3d R1 = Rs[imu_j] * ric[0];
            Eigen::Vector3d t = R0.transpose() * (t1 - t0);
            Eigen::Matrix3d R = R0.transpose() * R1;
            Eigen::Matrix<double, 3, 4> P;
            P.leftCols<3>() = R.transpose();
            P.rightCols<1>() = -R.transpose() * t;
            Eigen::Vector3d f = it_per_frame.point.normalized();
            svd_A.row(svd_idx++) = f[0] * P.row(2) - f[2] * P.row(0);
            svd_A.row(svd_idx++) = f[1] * P.row(2) - f[2] * P.row(1);

            if (imu_i == imu_j)
                continue;
        }
        ASSERT(svd_idx == svd_A.rows());
        Eigen::Vector4d svd_V = Eigen::JacobiSVD<Eigen::MatrixXd>(svd_A, Eigen::ComputeThinV).matrixV().rightCols<1>();
        double depth = svd_V[2] / svd_V[3];
        if (depth < 0.1)depth = INIT_DEPTH;
        it_per_id.idepth_ = 1. / depth;
        it_per_id.ptsInWorld = Rs[it_per_id.start_frame] * (
                                   ric[0] * (it_per_id.feature_per_frame[0].point / it_per_id.idepth_) + tic[0] - Pbg_
                               ) + Ps[it_per_id.start_frame];
        it_per_id.valid = true;
    }
}


void FeatureManager::removeBack(Eigen::Vector3d P0, Eigen::Matrix3d R0, Eigen::Vector3d P1, Eigen::Matrix3d R1, Eigen::Vector3d tic0, Eigen::Matrix3d ric0, Eigen::Vector3d Pbg_, ceres::Problem& my_problem) {
    for (auto it = feature.begin(), it_next = feature.begin();
            it != feature.end(); it = it_next) {
        it_next++;

        if (it->start_frame != 0)
            it->start_frame--;
        else {
#if USE_INVERSE_DEPTH
            Eigen::Vector3d ptsInW = R0 * (ric0 * (it->feature_per_frame[0].point / it->idepth_) + tic0 - Pbg_) + P0;
#else
            Eigen::Vector3d ptsInW = it->ptsInWorld;
#endif
            // ASSERT((ptsInW-it->ptsInWorld).norm()<1e-3);
            Eigen::Vector3d pts_cj = ric0.transpose() * ( R1.transpose() * (ptsInW - P1) + Pbg_ - tic0);
            it->idepth_ = 1. / pts_cj.z();

            it->feature_per_frame.erase(it->feature_per_frame.begin());

            ASSERT(!my_problem.HasParameterBlock(&it->idepth_));
#if USE_INVERSE_DEPTH
            for (auto& it_per_frame : it->feature_per_frame)it_per_frame.is_in_optimize = false;
#endif
            if (it->feature_per_frame.size() == 0) {
                ASSERT(0);
                feature.erase(it);
            }
        }
    }
}

void FeatureManager::removeFront(int image_index, ceres::Problem& my_problem) {
    for (auto it = feature.begin(), it_next = feature.begin(); it != feature.end(); it = it_next) {
        it_next++;

        if (it->start_frame == image_index) {
            it->start_frame--;
        } else {
            int j = image_index - 1 - it->start_frame;
            if (it->endFrame() < image_index - 1)
                continue;
            it->feature_per_frame.erase(it->feature_per_frame.begin() + j);
#if USE_INVERSE_DEPTH
            if (j == 0) {
                ASSERT(!my_problem.HasParameterBlock(&it->idepth_));
                for (auto& it_per_frame : it->feature_per_frame)it_per_frame.is_in_optimize = false;
            }
#endif
            if (it->feature_per_frame.size() == 0) {
                feature.erase(it);
                ASSERT(0);
            }
        }
    }
}

void FeatureManager::removeOut(int windowsize, ceres::Problem& my_problem) {
    for (auto it = feature.begin(), it_next = feature.begin(); it != feature.end(); it = it_next) {
        it_next++;
        if (it->endFrame() != windowsize - 1 && it->feature_per_frame.size() < FEATURE_CONTINUE) {
            feature.erase(it);
        }
    }
}

void FeatureManager::removeOut2(int windowsize, ceres::Problem& my_problem) {
    for (auto it = feature.begin(), it_next = feature.begin(); it != feature.end(); it = it_next) {
        it_next++;
        if (it->feature_per_frame.size() < FEATURE_CONTINUE) {
            if (USE_GLOBAL_OPTIMIZATION) {
#if USE_INVERSE_DEPTH
                if (my_problem.HasParameterBlock(&it->idepth_)) {
                    if (FEATURE_CONTINUE == 2) ASSERT(0);
                    my_problem.RemoveParameterBlock(&it->idepth_);
                }
#else
                if (my_problem.HasParameterBlock(it->ptsInWorld.data()))
                    my_problem.RemoveParameterBlock(it->ptsInWorld.data());
#endif
                for (auto& it_per_frame : it->feature_per_frame) {
                    if (it_per_frame.is_in_optimize) {
                        it_per_frame.is_in_optimize = false;
                    }
                }
            }

        } else {
            if (USE_ASSERT && USE_GLOBAL_OPTIMIZATION && windowsize > 4 && !USE_STEREO && it->start_frame < FEATURE_WINDOW_SIZE - 3) {
#if USE_INVERSE_DEPTH
                for (auto& it_per_frame : it->feature_per_frame) {
                    ASSERT(it_per_frame.is_in_optimize);
                }
                ASSERT(my_problem.HasParameterBlock(&it->idepth_));
                std::vector<ceres::internal::ResidualBlock*>residual_blocks;
                my_problem.GetResidualBlocksForParameterBlock(&it->idepth_, &residual_blocks);
                ASSERT(residual_blocks.size() == it->feature_per_frame.size() - 1);
#else
                ASSERT(my_problem.HasParameterBlock(it->ptsInWorld.data()));
                std::vector<ceres::internal::ResidualBlock*>residual_blocks;
                my_problem.GetResidualBlocksForParameterBlock(it->ptsInWorld.data(), &residual_blocks);
                ASSERT(residual_blocks.size() == it->feature_per_frame.size() ||
                       residual_blocks.size() == it->feature_per_frame.size() - 1);
#endif
            }
        }
    }
}

double FeatureManager::compensatedParallax2(const FeaturePerId& it_per_id, int image_index) {
    //check the second last frame is keyframe or not
    //parallax betwwen seconde last frame and third last frame
    const FeaturePerFrame& frame_i = it_per_id.feature_per_frame[image_index - 2 - it_per_id.start_frame];
    const FeaturePerFrame& frame_j = it_per_id.feature_per_frame[image_index - 1 - it_per_id.start_frame];

    double ans = 0;
    Eigen::Vector3d p_j = frame_j.point;

    double u_j = p_j(0);
    double v_j = p_j(1);

    Eigen::Vector3d p_i = frame_i.point;
    Eigen::Vector3d p_i_comp;

    p_i_comp = p_i;
    double dep_i = p_i(2);
    double u_i = p_i(0) / dep_i;
    double v_i = p_i(1) / dep_i;
    double du = u_i - u_j, dv = v_i - v_j;

    double dep_i_comp = p_i_comp(2);
    double u_i_comp = p_i_comp(0) / dep_i_comp;
    double v_i_comp = p_i_comp(1) / dep_i_comp;
    double du_comp = u_i_comp - u_j, dv_comp = v_i_comp - v_j;

    ans = max(ans, sqrt(min(du * du + dv * dv, du_comp * du_comp + dv_comp * dv_comp)));

    return ans;
}
